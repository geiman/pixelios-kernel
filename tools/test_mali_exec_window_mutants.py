"""Mutation test of the 16 MB executable window in xnu patch 0149 (bsd/dev/sweet_mali_pt.h mali_va_align; CR-02).

The header under test is 0104's new file with 0149's hunk applied (the same text shim/build.sh gives the host test). Each rule of
mali_va_align() is weakened in a copy (every occurrence of the pattern), shim/mali_pt_host_test.c is built against the copy and must FAIL;
a survivor fails this test, and so does a pattern that matches nothing (the header changed under the mutant list).
The refusal in sweet_mali_map() itself (sweet_mali.c, kernel only) has no host build; review covers it.
Run: python3 -B tools/test_mali_exec_window_mutants.py  (also picked up by `unittest discover -s tools -p 'test_*.py'`)."""
import os
import resource
import shutil
import subprocess
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PATCH0104 = os.path.join(ROOT, "xnu", "patches", "0104-sweet-mali-driver-core.patch")
PATCH0149 = os.path.join(ROOT, "xnu", "patches", "0149-sweet-mali-exec-16m-window.patch")
TEST = os.path.join(ROOT, "shim", "mali_pt_host_test.c")
HDR = "bsd/dev/sweet_mali_pt.h"

MUTANTS = [
    ("data mappings get the exec alignment", [("if (!(flags & MALI_F_EXEC)) {\n\t\treturn base_align;", "if (0) {\n\t\treturn base_align;")]),
    ("exec over 16 MB accepted", [("npages > MALI_EXEC_WINDOW_PAGES", "npages > 2 * MALI_EXEC_WINDOW_PAGES")]),
    ("exec of 0 pages accepted", [("if (npages == 0 || npages >", "if (npages >")]),
    ("alignment not raised to the size", [("while (a < npages) {", "while (0) {")]),
    ("alignment one step short", [("while (a < npages) {", "while (2 * a < npages) {")]),
    ("window of 8 MB", [("((16ull << 20) / MALI_PG)", "((8ull << 20) / MALI_PG)")]),
]


def new_file(patch, path):
    """One new-file hunk of a patch: the awk extraction of shim/build.sh."""
    out, on = [], False
    with open(patch, encoding="utf-8") as fh:
        for line in fh.read().split("\n"):
            if line == "+++ b/" + path:
                on = True
            elif on and (line.startswith("diff ") or line.startswith("--- ")):
                break
            elif on and line.startswith("@@"):
                continue
            elif on and line.startswith("+"):
                out.append(line[1:])
    if not out:
        raise ValueError("no new-file hunk for %s in %s" % (path, patch))
    return "\n".join(out) + "\n"


def file_diff(patch, path):
    out, on = [], False
    with open(patch, encoding="utf-8") as fh:
        for line in fh.read().split("\n"):
            if line.startswith("diff --git "):
                on = line.endswith(" b/" + path)
            if on:
                out.append(line)
    if not out:
        raise ValueError("%s does not touch %s" % (patch, path))
    return "\n".join(out) + "\n"


def header():
    with tempfile.TemporaryDirectory() as d:
        full = os.path.join(d, HDR)
        os.makedirs(os.path.dirname(full))
        with open(full, "w", encoding="utf-8") as fh:
            fh.write(new_file(PATCH0104, HDR))
        r = subprocess.run(["patch", "-s", "-p1", "-d", d], input=file_diff(PATCH0149, HDR), capture_output=True, text=True)
        if r.returncode != 0:
            raise ValueError("0149 does not apply to %s: %s" % (HDR, r.stdout + r.stderr))
        with open(full, encoding="utf-8") as fh:
            return fh.read()


def build_and_run(text, work):
    d = tempfile.mkdtemp(dir=work)
    with open(os.path.join(d, "sweet_mali_pt.h"), "w", encoding="utf-8") as fh:
        fh.write(text)
    shutil.copy(TEST, d)
    exe = os.path.join(d, "t")
    c = subprocess.run(["cc", "-O1", "-Wall", "-Wextra", "-Werror", "-I" + d, "-o", exe, os.path.join(d, "mali_pt_host_test.c")],
                       capture_output=True, text=True)
    if c.returncode != 0:
        return "compile", c.stderr[-300:]

    def cap():
        resource.setrlimit(resource.RLIMIT_FSIZE, (1 << 20, 1 << 20))
    r = subprocess.run([exe], capture_output=True, timeout=60, preexec_fn=cap)
    return r.returncode, r.stderr[-300:].decode("utf-8", "replace")


class MaliExecWindowMutants(unittest.TestCase):
    def test_unmutated_passes(self):
        with tempfile.TemporaryDirectory() as w:
            rc, err = build_and_run(header(), w)
            self.assertEqual(rc, 0, err)

    def test_every_mutant_is_killed(self):
        base = header()
        with tempfile.TemporaryDirectory() as w:
            for name, subs in MUTANTS:
                text = base
                for old, new in subs:
                    if old not in text:
                        self.fail("mutant %r: pattern not found (header changed?): %r" % (name, old))
                    text = text.replace(old, new)
                rc, _ = build_and_run(text, w)
                self.assertNotEqual(rc, 0, "SURVIVOR: %s" % name)


if __name__ == "__main__":
    unittest.main()
