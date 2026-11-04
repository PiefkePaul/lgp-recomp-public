#!/usr/bin/env python3
"""Export a public snapshot of the repository: one commit, no history, no branches.

The working repository stays private. This tool copies one revision (default
HEAD) into a new folder, leaves out files that only make sense for the private
project (agent instructions, project setup), and refuses the result if

  1. the game-data guard (tools/guard/check_game_data.py) finds anything,
  2. a file contains a private path pattern (Windows user folders, macOS volumes),
  3. a file contains a literal from the deny list,
  4. a file still names one of the decryption parts left out for legal reasons,
  5. a file has unbalanced public markers (below).

Decryption of game dumps (the kit page and its CLI, the nstool wrapper) stays in
the private repository: circumventing the encryption is not allowed in the EU
(Art. 6 InfoSoc directive, section 95a UrhG), own keys or not, and making such a
tool available is prohibited outright. Lines between "lgp-public: omit-start" and
"lgp-public: omit-end" (any comment syntax, marker lines included) are dropped
from text files; in Markdown, "<!-- lgp-public: only-start" ... "lgp-public:
only-end -->" holds text that only the public snapshot shows (the two marker
lines are dropped, the text between them stays).

The deny list holds the private strings themselves (user names, device names,
LAN addresses, team IDs), one per line, '#' starts a comment. It is read from
--deny or $LGP_PRIVATE_DIR/publish-deny.txt and never committed.

With --git the folder becomes a fresh git repository with a single commit;
pushing it is left to the person publishing.

--private-kit writes the private build kit for friends instead (repository
lgp-kit, never public): the same tree with the decryption parts kept, omit
blocks kept (only their marker lines dropped) and public-only blocks dropped.
Checks 1-3 and 5 still apply.

Exit code 0 = clean snapshot written, 1 = findings (folder is kept for
inspection), 2 = usage/internal error. Standard library only.
"""

from __future__ import annotations

import argparse
import io
import os
import posixpath
import re
import subprocess
import sys
import tarfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
GUARD = REPO / "tools" / "guard" / "check_game_data.py"

# Paths (prefix match on the POSIX path) that stay in the private repository.
DEFAULT_EXCLUDES = (
    "CLAUDE.md",
    "docs/project-setup/",
    ".claude/",
    # The workflows run on the self-hosted NAS runners; a public repository must never carry them (fork PRs
    # would run on them). GitHub-hosted workflows for the public repository come later, with Paul's approval.
    ".github/workflows/",
    # Internal measurement notes and agent notes: name people, machines and the NAS (Paul, 2026-11-02).
    # Other docs still mention these paths; in the public tree they point at notes that stay private.
    "docs/analysis/",
    "docs/agents/",
)

# Decryption of game dumps: never public (module docstring).
LEGAL_EXCLUDES = (
    "tools/extract/",
    "tools/kit/web/",
    "tools/kit/cli.mjs",
    "tools/kit/server.mjs",
    "tools/kit/Dockerfile",
    "tools/kit/docker-compose.yml",
    "tools/kit/.dockerignore",
    "tools/kit/README.md",
    "tools/kit/dump_input.py",
    "tests/kit/",
    "tests/tools/test_extract_exefs.py",
    "tests/tools/test_kit_dump.py",
    "docs/agents/bau-kit.md",
    "docs/knowledge/switch-dumps-im-browser-entschluesseln.md",
    "tools/kit/guide-images/dump/",  # screenshots of the dump path (NSP and keys)
)

# Names of the left-out decryption parts; an exported file that still names one is refused.
LEGAL_REFERENCES = (
    "tools/kit/web",
    "cli.mjs",
    "server.mjs",
    "extract_exefs",
    "dump_input",
    "test_kit_dump",
    "tests/kit",
    "bau-kit.md",
    "switch-dumps-im-browser-entschluesseln",
    "guide-images/dump",
)

# This tool and its test name the markers and the parts themselves.
MARKER_EXEMPT = ("tools/publish/public_snapshot.py", "tests/tools/test_public_snapshot.py")

OMIT_START, OMIT_END = "lgp-public: omit-start", "lgp-public: omit-end"
ONLY_START, ONLY_END = "<!-- lgp-public: only-start", "lgp-public: only-end -->"

# Private path shapes. Placeholders such as C:\Users\<name> are allowed.
PRIVATE_PATTERNS = (
    re.compile(r"[A-Za-z]:\\Users\\(?!<)[^\\\s`\"']+"),
    re.compile(r"/Users/(?!<|\$|runner\b|Shared\b)[A-Za-z0-9._-]{2,}/"),
    re.compile(r"/Volumes/(?!<|\$)[A-Za-z0-9._-]{2,}"),
)


def excluded(path: str, excludes: tuple[str, ...]) -> bool:
    return any(path == e or (e.endswith("/") and path.startswith(e)) for e in excludes)


def read_deny(path: Path | None) -> list[str]:
    if path is None or not path.is_file():
        return []
    out = []
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if line and not line.startswith("#"):
            out.append(line)
    return out


def export(ref: str, out: Path, excludes: tuple[str, ...], executables: list[str] | None = None) -> list[str]:
    """Writes the files of ref into out; the names of executable ones (100755 in git) go into executables."""
    data = subprocess.run(["git", "-C", str(REPO), "archive", "--format=tar", ref],
                          check=True, capture_output=True).stdout
    written = []
    with tarfile.open(fileobj=io.BytesIO(data)) as tar:
        for member in tar.getmembers():
            if not member.isfile() or excluded(member.name, excludes):
                continue
            target = out / member.name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(tar.extractfile(member).read())
            os.chmod(target, member.mode & 0o777 or 0o644)
            written.append(member.name)
            if executables is not None and member.mode & 0o111:
                executables.append(member.name)
    return written


_LINK = r"\[(?P<text>[^\]\n]*)\]\((?P<target>[^)\s]+)\)"
LINK_RE = re.compile(_LINK)
# "([13](a), [23](b))": a parenthesized list of links and nothing else.
LINK_GROUP_RE = re.compile(r" ?\((?:" + _LINK.replace("?P<text>", "?:").replace("?P<target>", "?:") +
                           r"(?:\s*[,;]\s*)?)+\)")
# A bare number such as [54] says nothing once its target is gone.
NUMBER_TEXT_RE = re.compile(r"\d+[a-z]?")
NUMBER_AFTER_COMMA_RE = re.compile(r"[,;] " + _LINK)
NUMBER_OPENING_LIST_RE = re.compile(r"(?<=\()" + _LINK + r"[,;] ")


def link_target_left_out(name: str, target: str, excludes: tuple[str, ...]) -> bool:
    """True if a relative Markdown link in file name points at a path the snapshot leaves out."""
    if re.match(r"[A-Za-z][A-Za-z0-9+.-]*:", target) or target.startswith(("#", "/")):
        return False
    path = target.split("#", 1)[0]
    if not path:
        return False
    resolved = posixpath.normpath(posixpath.join(posixpath.dirname(name), path))
    if resolved.startswith("../"):
        return False
    return excluded(resolved, excludes) or excluded(resolved + "/", excludes)


def unlink_left_out(name: str, text: str, excludes: tuple[str, ...]) -> tuple[str, int]:
    """Markdown links into left-out paths lose their link: a parenthesized list of only such links goes, a bare
    number goes with the comma before it, any other link keeps its text. Returns the text and the links removed."""
    count = 0

    def gone(target: str) -> bool:
        return link_target_left_out(name, target, excludes)

    def group(m: re.Match) -> str:
        nonlocal count
        targets = [lm.group("target") for lm in LINK_RE.finditer(m.group(0))]
        if not all(gone(t) for t in targets):
            return m.group(0)
        count += len(targets)
        return ""

    def single(m: re.Match) -> str:
        nonlocal count
        if not gone(m.group("target")):
            return m.group(0)
        count += 1
        return "" if NUMBER_TEXT_RE.fullmatch(m.group("text")) else m.group("text")

    text = LINK_GROUP_RE.sub(group, text)
    for pattern in (NUMBER_OPENING_LIST_RE, NUMBER_AFTER_COMMA_RE):
        text = pattern.sub(
            lambda m: single(m) if gone(m.group("target")) and NUMBER_TEXT_RE.fullmatch(m.group("text"))
            else m.group(0), text)
    text = LINK_RE.sub(single, text)
    return text, count


def apply_markers(text: str, private_kit: bool = False) -> tuple[str, list[str]]:
    """Drop omit blocks, unwrap public-only blocks; returns the new text and marker errors.
    private_kit: the other way round, keep the omit blocks' text and drop the public-only blocks."""
    lines = text.splitlines(keepends=True)
    kept: list[str] = []
    errors: list[str] = []
    mode = None  # None, "omit" or "only"
    start = 0
    for lineno, line in enumerate(lines, 1):
        marker = None
        for name, token in (("omit-start", OMIT_START), ("omit-end", OMIT_END),
                            ("only-start", ONLY_START), ("only-end", ONLY_END)):
            if token in line:
                marker = name
                break
        if marker is None:
            if mode != ("only" if private_kit else "omit"):
                kept.append(line)
            continue
        if marker.endswith("-start"):
            if mode is not None:
                errors.append(f"{lineno}: {marker} inside an open block from line {start}")
            mode, start = marker[:-6], lineno
        elif mode != marker[:-4]:
            errors.append(f"{lineno}: {marker} without its start")
        else:
            mode = None
    if mode is not None:
        errors.append(f"{start}: {mode}-start is never closed")
    return "".join(kept), errors


def scan(out: Path, files: list[str], deny: list[str], excludes: tuple[str, ...] = (),
         unlinked: list[int] | None = None, private_kit: bool = False) -> list[str]:
    findings = []
    for name in files:
        try:
            text = (out / name).read_bytes().decode("utf-8")
        except (UnicodeDecodeError, OSError):
            continue
        exempt = name in MARKER_EXEMPT
        changed = False
        if not exempt and "lgp-public:" in text:
            text, errors = apply_markers(text, private_kit)
            findings += [f"{name}:{e}" for e in errors]
            changed = True
        if name.endswith(".md") and excludes:
            text, n = unlink_left_out(name, text, excludes)
            if n:
                changed = True
                if unlinked is not None:
                    unlinked.append(n)
        if changed:
            (out / name).write_bytes(text.encode("utf-8"))
        for lineno, line in enumerate(text.splitlines(), 1):
            for ref in () if exempt or private_kit else LEGAL_REFERENCES:
                if ref in line:
                    findings.append(f"{name}:{lineno}: names left-out decryption part {ref!r}")
            for pattern in PRIVATE_PATTERNS:
                m = pattern.search(line)
                if m:
                    findings.append(f"{name}:{lineno}: private path {m.group(0)!r}")
            for literal in deny:
                if literal in line:
                    # Do not echo the literal: the report may end up in a log.
                    findings.append(f"{name}:{lineno}: deny-list entry #{deny.index(literal) + 1}")
    return findings


def guard(out: Path, files: list[str], max_chars: int = 8000) -> int:
    """Run the guard in batches: Windows command lines end at 32767 characters."""
    worst = 0
    batch: list[str] = []
    size = 0
    for name in [*files, None]:
        if batch and (name is None or size + len(name) + 1 > max_chars):
            rc = subprocess.run([sys.executable, "-I", str(GUARD), *batch], cwd=out).returncode
            worst = max(worst, rc)
            batch, size = [], 0
        if name is not None:
            batch.append(name)
            size += len(name) + 1
    return worst


def make_git(out: Path, message: str, executables: list[str] = ()) -> None:
    subprocess.run(["git", "init", "-q", "-b", "main"], cwd=out, check=True)
    subprocess.run(["git", "add", "-A"], cwd=out, check=True)
    # Windows checks out with core.filemode=false: git add records every file as 100644, so the modes of the source
    # repository go into the index explicitly (lgp-bauen.command, *.sh must stay executable).
    for i in range(0, len(executables), 200):
        subprocess.run(["git", "update-index", "--chmod=+x", "--", *executables[i:i + 200]], cwd=out, check=True)
    identity = []
    # Without a configured identity (CI, fresh machines) commit as the tool.
    if subprocess.run(["git", "config", "user.email"], cwd=out, capture_output=True).returncode != 0:
        identity = ["-c", "user.name=lgp-recomp snapshot", "-c", "user.email=snapshot@lgp-recomp.invalid"]
    subprocess.run(["git", *identity, "commit", "-q", "-m", message], cwd=out, check=True)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--out", required=True, type=Path, help="new or empty folder")
    ap.add_argument("--ref", default="HEAD", help="revision to export (default HEAD)")
    ap.add_argument("--deny", type=Path, help="deny list (default $LGP_PRIVATE_DIR/publish-deny.txt)")
    ap.add_argument("--exclude", action="append", default=[],
                    help="extra path or folder/ to leave out (repeatable)")
    ap.add_argument("--git", action="store_true", help="init a fresh repository with one commit")
    ap.add_argument("--message", help="commit message with --git (default 'Public snapshot' or 'Private kit snapshot')")
    ap.add_argument("--private-kit", action="store_true",
                    help="the private build kit (lgp-kit): keep the decryption parts and the omit blocks")
    args = ap.parse_args(argv)

    if args.out.exists() and any(args.out.iterdir()):
        print(f"error: {args.out} is not empty", file=sys.stderr)
        return 2
    deny_path = args.deny
    if deny_path is None and os.environ.get("LGP_PRIVATE_DIR"):
        deny_path = Path(os.environ["LGP_PRIVATE_DIR"]) / "publish-deny.txt"
    if args.deny is not None and not args.deny.is_file():
        print(f"error: deny list {args.deny} not found", file=sys.stderr)
        return 2
    deny = read_deny(deny_path)
    if not deny:
        print("warning: no deny list, only built-in path patterns are checked", file=sys.stderr)

    args.out.mkdir(parents=True, exist_ok=True)
    excludes = DEFAULT_EXCLUDES + (() if args.private_kit else LEGAL_EXCLUDES) + tuple(args.exclude)
    try:
        executables: list[str] = []
        files = export(args.ref, args.out, excludes, executables)
    except subprocess.CalledProcessError as e:
        print(f"error: git archive {args.ref} failed: {e.stderr.decode(errors='replace')}", file=sys.stderr)
        return 2

    unlinked: list[int] = []
    findings = scan(args.out, files, deny, excludes, unlinked, args.private_kit)
    if unlinked:
        print(f"links into left-out paths removed: {sum(unlinked)} in {len(unlinked)} files")
    for f in findings:
        print(f)
    guard_rc = guard(args.out, files)
    if findings or guard_rc != 0:
        print(f"snapshot refused: {len(findings)} private finding(s), guard exit {guard_rc}; "
              f"folder kept at {args.out}", file=sys.stderr)
        return 1
    if args.git:
        make_git(args.out, args.message or ("Private kit snapshot" if args.private_kit else "Public snapshot"),
                 executables)
    print(f"{'private kit ' if args.private_kit else ''}snapshot ok: {len(files)} files from {args.ref} in {args.out}, "
          f"{len(deny)} deny-list entries checked")
    return 0


if __name__ == "__main__":
    sys.exit(main())
