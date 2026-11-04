"""Unit test for tools/publish/public_snapshot.py on a throwaway git repository.

Run: python -m unittest -v test_public_snapshot (from tests/tools)
"""
import io
import os
import subprocess
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path

HERE = os.path.dirname(__file__)
sys.path.insert(0, os.path.join(HERE, "..", "..", "tools", "publish"))
import public_snapshot as ps  # noqa: E402


def git(cwd, *args):
    subprocess.run(["git", *args], cwd=cwd, check=True, capture_output=True)


class PublicSnapshotTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        root = Path(self.tmp.name)
        self.repo = root / "repo"
        self.repo.mkdir()
        git(self.repo, "init", "-q", "-b", "main")
        git(self.repo, "config", "user.email", "t@example.invalid")
        git(self.repo, "config", "user.name", "t")
        (self.repo / "README.md").write_text("hello\nC:\\Users\\<name>\\tools is fine\n")
        (self.repo / "CLAUDE.md").write_text("private\n")
        (self.repo / "docs" / "project-setup").mkdir(parents=True)
        (self.repo / "docs" / "project-setup" / "a.md").write_text("private\n")
        (self.repo / "run.command").write_text("#!/bin/sh\n")
        (self.repo / ".github" / "workflows").mkdir(parents=True)
        (self.repo / ".github" / "workflows" / "ci.yml").write_text("runs-on: [self-hosted, nas]\n")
        (self.repo / ".github" / "pull_request_template.md").write_text("template\n")
        for d in ("analysis", "agents", "knowledge"):
            (self.repo / "docs" / d).mkdir(parents=True)
            (self.repo / "docs" / d / "note.md").write_text("note\n")
        (self.repo / "docs" / "knowledge" / "links.md").write_text(
            "Measured ([24](../analysis/note.md)). Status in [`note.md`](../analysis/note.md#x), "
            "kept [here](note.md) and [web](https://example.invalid/docs/analysis/).\n")
        git(self.repo, "add", "-A")
        git(self.repo, "update-index", "--chmod=+x", "run.command")
        git(self.repo, "commit", "-q", "-m", "init")
        self.out = root / "out"
        self.deny = root / "deny.txt"
        self.deny.write_text("# comment\nsecret-host\n")
        self._old_repo = ps.REPO
        ps.REPO = self.repo

    def tearDown(self):
        ps.REPO = self._old_repo
        self.tmp.cleanup()

    def run_tool(self, *extra):
        buf = io.StringIO()
        with redirect_stdout(buf):
            rc = ps.main(["--out", str(self.out), "--deny", str(self.deny), *extra])
        return rc, buf.getvalue()

    def test_clean_snapshot_leaves_out_private_files(self):
        rc, _ = self.run_tool("--git")
        self.assertEqual(rc, 0)
        self.assertTrue((self.out / "README.md").is_file())
        self.assertFalse((self.out / "CLAUDE.md").exists())
        self.assertFalse((self.out / "docs" / "project-setup").exists())
        log = subprocess.run(["git", "log", "--oneline"], cwd=self.out, capture_output=True,
                             text=True, check=True).stdout.splitlines()
        self.assertEqual(len(log), 1)

    def test_private_path_is_refused(self):
        (self.repo / "notes.md").write_text("see C:\\Users\\someone\\stuff\n")
        git(self.repo, "add", "-A")
        git(self.repo, "commit", "-q", "-m", "leak")
        rc, out = self.run_tool()
        self.assertEqual(rc, 1)
        self.assertIn("notes.md:1: private path", out)

    def test_deny_literal_is_refused_without_echo(self):
        (self.repo / "net.md").write_text("server secret-host here\n")
        git(self.repo, "add", "-A")
        git(self.repo, "commit", "-q", "-m", "leak")
        rc, out = self.run_tool()
        self.assertEqual(rc, 1)
        self.assertIn("net.md:1: deny-list entry #1", out)
        self.assertNotIn("secret-host", out)

    def test_game_data_is_refused_by_guard(self):
        (self.repo / "game.nsp").write_bytes(b"PFS0" + bytes(64))
        git(self.repo, "add", "-A")
        git(self.repo, "commit", "-q", "-m", "leak")
        rc, _ = self.run_tool()
        self.assertEqual(rc, 1)

    def test_guard_runs_in_batches(self):
        (self.repo / "game.nsp").write_bytes(b"PFS0" + bytes(64))
        git(self.repo, "add", "-A")
        git(self.repo, "commit", "-q", "-m", "leak")
        self.out.mkdir()
        files = ps.export("HEAD", self.out, ps.DEFAULT_EXCLUDES)
        self.assertEqual(ps.guard(self.out, sorted(files), max_chars=1), 1)
        clean = [f for f in files if f != "game.nsp"]
        self.assertEqual(ps.guard(self.out, clean, max_chars=1), 0)

    def test_decryption_parts_are_left_out(self):
        (self.repo / "tools" / "kit" / "web" / "lib").mkdir(parents=True)
        (self.repo / "tools" / "kit" / "web" / "lib" / "nca.js").write_text("// decrypt\n")
        (self.repo / "tools" / "kit" / "build.py").write_text("# builds\n")
        git(self.repo, "add", "-A")
        git(self.repo, "commit", "-q", "-m", "kit")
        rc, _ = self.run_tool()
        self.assertEqual(rc, 0)
        self.assertFalse((self.out / "tools" / "kit" / "web").exists())
        self.assertTrue((self.out / "tools" / "kit" / "build.py").is_file())

    def test_markers_drop_and_unwrap(self):
        (self.repo / "doc.md").write_text(
            "a\n<!-- lgp-public: omit-start -->\nrun cli.mjs extract\n<!-- lgp-public: omit-end -->\n"
            "<!-- lgp-public: only-start\npublic note\nlgp-public: only-end -->\nb\n")
        (self.repo / "CMakeLists.txt").write_text(
            "x()\n# lgp-public: omit-start\nadd_subdirectory(kit)\n# lgp-public: omit-end\n")
        git(self.repo, "add", "-A")
        git(self.repo, "commit", "-q", "-m", "markers")
        rc, _ = self.run_tool()
        self.assertEqual(rc, 0)
        self.assertEqual((self.out / "doc.md").read_text(), "a\npublic note\nb\n")
        self.assertEqual((self.out / "CMakeLists.txt").read_text(), "x()\n")

    def test_private_kit_keeps_the_decryption(self):
        """--private-kit (lgp-kit, never public): decryption parts and omit blocks stay, public-only blocks go,
        the internal notes and the private-path and deny checks stay as for the public snapshot."""
        (self.repo / "tools" / "kit" / "web" / "lib").mkdir(parents=True)
        (self.repo / "tools" / "kit" / "web" / "lib" / "nca.js").write_text("// decrypt\n")
        (self.repo / "doc.md").write_text(
            "a\n<!-- lgp-public: omit-start -->\nrun cli.mjs extract\n<!-- lgp-public: omit-end -->\n"
            "<!-- lgp-public: only-start\npublic note\nlgp-public: only-end -->\nb\n")
        git(self.repo, "add", "-A")
        git(self.repo, "commit", "-q", "-m", "kit")
        rc, out = self.run_tool("--private-kit", "--git")
        self.assertEqual(rc, 0, out)
        self.assertIn("private kit snapshot ok", out)
        self.assertTrue((self.out / "tools" / "kit" / "web" / "lib" / "nca.js").is_file())
        self.assertEqual((self.out / "doc.md").read_text(), "a\nrun cli.mjs extract\nb\n")
        for gone in ("CLAUDE.md", "docs/project-setup", "docs/analysis", "docs/agents", ".github/workflows"):
            self.assertFalse((self.out / gone).exists(), gone)
        msg = subprocess.run(["git", "log", "--format=%s"], cwd=self.out, capture_output=True, text=True,
                             check=True).stdout.strip()
        self.assertEqual(msg, "Private kit snapshot")

    def test_private_kit_still_refuses_private_strings(self):
        (self.repo / "net.md").write_text("<!-- lgp-public: omit-start -->\nserver secret-host\n"
                                          "<!-- lgp-public: omit-end -->\n")
        git(self.repo, "add", "-A")
        git(self.repo, "commit", "-q", "-m", "leak")
        rc, out = self.run_tool("--private-kit")
        self.assertEqual(rc, 1)
        self.assertIn("net.md:1: deny-list entry #1", out)  # line of the written file

    def test_reference_to_left_out_part_is_refused(self):
        (self.repo / "how.md").write_text("node tools/kit/cli.mjs extract\n")
        git(self.repo, "add", "-A")
        git(self.repo, "commit", "-q", "-m", "leak")
        rc, out = self.run_tool()
        self.assertEqual(rc, 1)
        self.assertIn("how.md:1: names left-out decryption part", out)

    def test_unbalanced_marker_is_refused(self):
        (self.repo / "open.md").write_text("<!-- lgp-public: omit-start -->\nrest\n")
        git(self.repo, "add", "-A")
        git(self.repo, "commit", "-q", "-m", "open")
        rc, out = self.run_tool()
        self.assertEqual(rc, 1)
        self.assertIn("open.md:1: omit-start is never closed", out)

    def test_git_keeps_executable_modes_with_filemode_off(self):
        """PC2 on #543: on Windows (core.filemode=false) git add made every file 100644 in the snapshot."""
        real = ps.make_git

        def without_filemode(out, message, executables=()):
            git(out, "init", "-q", "-b", "main")
            git(out, "config", "core.filemode", "false")
            for f in executables:  # what a Windows file system shows: no executable bit
                os.chmod(out / f, 0o644)
            real(out, message, executables)

        ps.make_git = without_filemode
        try:
            rc, out = self.run_tool("--git")
        finally:
            ps.make_git = real
        self.assertEqual(rc, 0, out)
        modes = subprocess.run(["git", "ls-files", "-s"], cwd=self.out, capture_output=True, text=True,
                               check=True).stdout
        self.assertIn("100755", next(line for line in modes.splitlines() if line.endswith("run.command")))
        self.assertIn("100644", next(line for line in modes.splitlines() if line.endswith("README.md")))

    def test_workflows_are_left_out_without_a_deny_list(self):
        self.deny.write_text("")
        rc, out = self.run_tool()
        self.assertEqual(rc, 0, out)
        self.assertFalse((self.out / ".github" / "workflows").exists())
        self.assertTrue((self.out / ".github" / "pull_request_template.md").is_file())

    def test_internal_notes_are_left_out(self):
        self.deny.write_text("")
        rc, out = self.run_tool()
        self.assertEqual(rc, 0, out)
        self.assertFalse((self.out / "docs" / "analysis").exists())
        self.assertFalse((self.out / "docs" / "agents").exists())
        self.assertTrue((self.out / "docs" / "knowledge" / "note.md").is_file())
        self.assertEqual((self.out / "docs" / "knowledge" / "links.md").read_text(),
                         "Measured. Status in `note.md`, kept [here](note.md) and "
                         "[web](https://example.invalid/docs/analysis/).\n")
        self.assertIn("links into left-out paths removed: 2 in 1 files", out)

    def test_non_empty_out_is_rejected(self):
        self.out.mkdir()
        (self.out / "x").write_text("x")
        rc, _ = self.run_tool()
        self.assertEqual(rc, 2)


class UnlinkTest(unittest.TestCase):
    EX = ("docs/analysis/", "CLAUDE.md")

    def unlink(self, text, name="README.md"):
        return ps.unlink_left_out(name, text, self.EX)

    def test_link_lists_and_numbers_go(self):
        self.assertEqual(self.unlink("passed ([13](docs/analysis/a.md), [23](docs/analysis/b.md)). Next"),
                         ("passed. Next", 2))
        self.assertEqual(self.unlink("as in Yellow, [54](docs/analysis/c.md))"), ("as in Yellow)", 1))

    def test_other_links_keep_their_text(self):
        self.assertEqual(self.unlink("[Analysis 13](docs/analysis/a.md), [ADR](docs/decisions/1.md)"),
                         ("Analysis 13, [ADR](docs/decisions/1.md)", 1))
        self.assertEqual(self.unlink("see [rules](CLAUDE.md)"), ("see rules", 1))
        self.assertEqual(self.unlink("([28](docs/analysis/a.md), [ADR](docs/decisions/1.md))"),
                         ("([ADR](docs/decisions/1.md))", 1))

    def test_relative_paths_resolve_from_the_file(self):
        self.assertEqual(self.unlink("[x](../../docs/analysis/a.md)", "tests/fixtures/README.md"), ("x", 1))
        self.assertEqual(self.unlink("[x](analysis/a.md)", "docs/roadmap.md"), ("x", 1))
        self.assertEqual(self.unlink("[x](analysis/a.md)", "README.md"), ("[x](analysis/a.md)", 0))


if __name__ == "__main__":
    unittest.main()
