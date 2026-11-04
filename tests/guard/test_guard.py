"""Tests for tools/guard/check_game_data.py and the .githooks/pre-commit hook.

The end-to-end tests create a throwaway git repository, install the real hook
and guard into it, and prove that `git commit` is refused for game data.
All "game data" here is synthetic: a few magic bytes plus zero padding.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
GUARD_DIR = REPO_ROOT / "tools" / "guard"
HOOKS_DIR = REPO_ROOT / ".githooks"

sys.path.insert(0, str(GUARD_DIR))
import check_game_data as guard  # noqa: E402


def fake_nca() -> bytes:
    # Decrypted NCA header: magic "NCA3" at offset 0x200.
    return b"\0" * 0x200 + b"NCA3" + b"\0" * 0x1FC


def fake_key_file() -> bytes:
    # Synthetic, not real keys: same line shape as prod.keys.
    lines = [f"fake_key_{i:02d} = {'%02x' % i * 16}" for i in range(5)]
    return ("\n".join(lines) + "\n").encode()


class GuardUnitTests(unittest.TestCase):
    def reasons(self, path: str, data: bytes, size: int | None = None) -> list[str]:
        return [v.reason for v in guard.check_file(path, len(data) if size is None else size, data)]

    def test_nca3_magic_rejected_regardless_of_name(self):
        self.assertTrue(any("NCA3" in r for r in self.reasons("docs/notes.txt", fake_nca())))

    def test_nso0_and_pfs0_magic_rejected(self):
        self.assertTrue(any("NSO0" in r for r in self.reasons("a.bin", b"NSO0" + b"\0" * 64)))
        self.assertTrue(any("PFS0" in r for r in self.reasons("a.bin", b"PFS0" + b"\0" * 64)))

    def test_forbidden_extensions_and_names(self):
        for name in ("game.nsp", "x/y.NCA", "main.nso", "prod.keys", "sub/title.keys", "a.xci"):
            self.assertTrue(self.reasons(name, b"harmless"), name)

    def test_pipeline_cache_rejected(self):
        # Derived from game shaders: by extension and, renamed, by its magic.
        self.assertTrue(self.reasons("private/cache/webgpu-pipelines.lgppipes", b"harmless"))
        cache = b"LGPPIPE1" + b"\x05\0\0\0" + b"x" * 64
        self.assertTrue(any("pipeline cache" in r for r in self.reasons("notes.bin", cache)))

    def test_metal_library_rejected(self):
        # Derived from game shaders (tools/apple/build_metallibs.sh): by
        # extension and, renamed, by its magic.
        self.assertTrue(self.reasons("private/metal/msl-macos/0123456789abcdef.metal", b"// lgp-msl: 3.2\n"))
        self.assertTrue(self.reasons("private/metal/macos/0123456789abcdef.metallib", b"harmless"))
        self.assertTrue(any("Metal library" in r for r in self.reasons("notes.bin", b"MTLB" + b"\0" * 60)))

    def test_size_limit(self):
        self.assertTrue(any("size" in r for r in self.reasons("big.dat", b"", guard.MAX_FILE_BYTES + 1)))

    def test_key_file_content_rejected(self):
        self.assertTrue(any("key file" in r for r in self.reasons("notes.txt", fake_key_file())))

    def test_homebrew_fixture_allowed_but_not_nca(self):
        nso = b"NSO0" + b"\0" * 64
        self.assertEqual(self.reasons("tests/fixtures/hello/hello.nso", nso), [])
        self.assertTrue(self.reasons("tests/fixtures/evil.nso", fake_nca()))

    def test_clean_source_passes(self):
        self.assertEqual(self.reasons("recomp/lifter.cpp", b"int main() { return 0; }\n"), [])


@unittest.skipUnless(shutil.which("git"), "git not installed")
class PreCommitHookTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.repo = Path(self.tmp.name)
        self.git("init", "-q")
        shutil.copytree(HOOKS_DIR, self.repo / ".githooks")
        shutil.copytree(GUARD_DIR, self.repo / "tools" / "guard",
                        ignore=shutil.ignore_patterns("__pycache__"))
        hook = self.repo / ".githooks" / "pre-commit"
        hook.chmod(hook.stat().st_mode | 0o111)
        self.git("config", "core.hooksPath", ".githooks")

    def tearDown(self):
        self.tmp.cleanup()

    def git(self, *args: str) -> subprocess.CompletedProcess:
        env = dict(os.environ, GIT_CONFIG_NOSYSTEM="1")
        return subprocess.run(
            ["git", "-c", "user.name=guard-test", "-c", "user.email=guard@test.invalid",
             "-c", "commit.gpgsign=false", *args],
            cwd=self.repo, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
        )

    def commit_file(self, rel: str, data: bytes) -> subprocess.CompletedProcess:
        path = self.repo / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        self.assertEqual(self.git("add", "-f", rel).returncode, 0)
        return self.git("commit", "-q", "-m", f"add {rel}")

    def test_commit_with_nca3_magic_is_rejected(self):
        # Innocent-looking name: only the magic bytes give it away.
        result = self.commit_file("docs/readme.txt", fake_nca())
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("NCA3", result.stderr)
        self.assertEqual(self.git("rev-parse", "--verify", "-q", "HEAD").returncode, 1,
                         "no commit must exist after rejection")

    def test_commit_of_forbidden_extension_is_rejected(self):
        result = self.commit_file("dump/game.nsp", b"x")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(".nsp", result.stderr)

    def test_clean_commit_passes(self):
        result = self.commit_file("src/main.cpp", b"int main() { return 0; }\n")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_staged_content_is_checked_not_worktree(self):
        path = self.repo / "notes.txt"
        path.write_bytes(fake_nca())
        self.git("add", "notes.txt")
        path.write_bytes(b"clean now\n")  # unstaged edit must not hide the staged blob
        result = self.git("commit", "-q", "-m", "sneaky")
        self.assertNotEqual(result.returncode, 0)


if __name__ == "__main__":
    unittest.main(verbosity=2)
