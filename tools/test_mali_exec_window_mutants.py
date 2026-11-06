"""Mutation test of the 16 MB executable window in xnu patch 0149 (bsd/dev/sweet_mali_pt.h mali_va_align; CR-02).

The header under test is 0104's new file with 0149's hunk applied (the same text shim/build.sh gives the host test). Each rule of
mali_va_align() is weakened in a copy (every occurrence of the pattern), shim/mali_pt_host_test.c is built against the copy and must FAIL;
a survivor fails this test, and so does a pattern that matches nothing (the header changed under the mutant list).
sweet_mali_map() itself (sweet_mali.c, kernel only, no host build) is covered by source pins (review 1 F1): its body must take
the VA from mali_va_alloc_map() with the one flag translation `mflags`, refuse with EINVAL on -1 after unlocking, and pass
`mflags` to every mali_pt_map(); each pin has a source mutant that must break it.
Run: python3 -B tools/test_mali_exec_window_mutants.py  (also picked up by `unittest discover -s tools -p 'test_*.py'`)."""
import os
import re
import resource
import shutil
import subprocess
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PATCH0104 = os.path.join(ROOT, "xnu", "patches", "0104-sweet-mali-driver-core.patch")
PATCH0107 = os.path.join(ROOT, "xnu", "patches", "0107-sweet-mali-validate-pa.patch")
PATCH0109 = os.path.join(ROOT, "xnu", "patches", "0109-sweet-gic-probe.patch")
PATCH0149 = os.path.join(ROOT, "xnu", "patches", "0149-sweet-mali-exec-16m-window.patch")
TEST = os.path.join(ROOT, "shim", "mali_pt_host_test.c")
HDR = "bsd/dev/sweet_mali_pt.h"
SRC = "bsd/dev/sweet_mali.c"

MUTANTS = [
    ("data mappings get the exec alignment", [("if (!(flags & MALI_F_EXEC)) {\n\t\treturn base_align;", "if (0) {\n\t\treturn base_align;")]),
    ("exec over 16 MB accepted", [("npages > MALI_EXEC_WINDOW_PAGES", "npages > 2 * MALI_EXEC_WINDOW_PAGES")]),
    ("exec of 0 pages accepted", [("if (npages == 0 || npages >", "if (npages >")]),
    ("alignment not raised to the size", [("while (a < npages) {", "while (0) {")]),
    ("alignment one step short", [("while (a < npages) {", "while (2 * a < npages) {")]),
    ("window of 8 MB", [("((16ull << 20) / MALI_PG)", "((8ull << 20) / MALI_PG)")]),
    ("alloc_map ignores the exec alignment", [("uint64_t x = mali_va_alloc(v, npages, a);", "uint64_t x = mali_va_alloc(v, npages, MALI_MAP_ALIGN_PAGES);")]),
    ("alloc_map does not refuse", [("if (a == 0) {\n\t\treturn -1;", "if (a == 0) {\n\t\ta = MALI_MAP_ALIGN_PAGES;")]),
    ("alloc_map data alignment 4 KB", [("#define MALI_MAP_ALIGN_PAGES    4u", "#define MALI_MAP_ALIGN_PAGES    1u")]),
    ("alloc_map no-room reported as refusal", [("if (x == 0) {\n\t\treturn -2;", "if (x == 0) {\n\t\treturn -1;")]),
]

# sweet_mali.c: the exact statements sweet_mali_map() must contain (source pins), and mutants each pin must catch
MFLAGS = "const uint32_t mflags = (flags & SWEET_MALI_EXEC) ? MALI_F_EXEC : 0;"
CALL = "int ae = mali_va_alloc_map(&sm.va, n * 4u, mflags, &va);"
SRC_MUTANTS = [
    ("flags translated to 0", [(MFLAGS, "const uint32_t mflags = 0;")]),
    ("call passes 0 instead of mflags", [(CALL, "int ae = mali_va_alloc_map(&sm.va, n * 4u, 0, &va);")]),
    ("fixed 16 KB alignment restored", [(CALL, "int ae = 0;\n\tva = mali_va_alloc(&sm.va, n * 4u, 4u);")]),
    ("refusal returns ENOMEM", [("\t\t\treturn EINVAL;\n\t\t}\n\t\treturn ENOMEM;", "\t\t\treturn ENOMEM;\n\t\t}\n\t\treturn ENOMEM;")]),
    ("refusal removed", [("\t\tif (ae == -1) {", "\t\tif (0) {")]),
    ("lock kept on failure", [("if (ae != 0) {\n\t\tlck_mtx_unlock(&sm_lock);", "if (ae != 0) {\n\t\t;")]),
    ("page entries ignore mflags", [("pa16k[i] + 0x1000ull * k, mflags);", "pa16k[i] + 0x1000ull * k, 0);")]),
    ("translation after the call", [(MFLAGS + "\n", ""), (CALL, CALL + "\n\t" + MFLAGS)]),
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
    """The diff of one file in a patch: from its '--- ' / '+++ b/<path>' pair to the next file's pair or 'diff --git' line.
    Works with and without 'diff --git' headers (0109 has none). Raises if the patch does not touch the file."""
    with open(patch, encoding="utf-8") as fh:
        lines = fh.read().split("\n")
    out, on = [], False
    for n, line in enumerate(lines):
        starts = line.startswith("--- ") and n + 1 < len(lines) and lines[n + 1].startswith("+++ ")
        if line.startswith("diff --git ") or starts:
            on = starts and lines[n + 1] == "+++ b/" + path
        if on:
            out.append(line)
    if not out:
        raise ValueError("%s does not touch %s" % (patch, path))
    return "\n".join(out) + "\n"


def apply_chain(path, patches):
    """The text of `path` after each patch's section for it is applied in order with patch(1)."""
    with tempfile.TemporaryDirectory() as d:
        full = os.path.join(d, path)
        os.makedirs(os.path.dirname(full))
        for pt in patches:
            sec = file_diff(pt, path)         # every listed patch must touch the file: no silent skip
            r = subprocess.run(["patch", "-s", "-p1", "-d", d], input=sec, capture_output=True, text=True)
            if r.returncode != 0:
                raise ValueError("%s does not apply to %s: %s" % (os.path.basename(pt), path, r.stdout + r.stderr))
        with open(full, encoding="utf-8") as fh:
            return fh.read()


def map_body(text):
    m = re.search(r"^sweet_mali_map\(.*?^}$", text, re.S | re.M)
    return m.group(0) if m else ""


def pin_errors(text):
    """Every way the sweet_mali_map() body departs from the pinned VA decision; [] when it holds."""
    b, errs = map_body(text), []
    if not b:
        return ["no sweet_mali_map body"]
    if b.count(MFLAGS) != 1:
        errs.append("flag translation not exactly once")
    if b.count(CALL) != 1:
        errs.append("mali_va_alloc_map call not exactly once")
    if "mali_va_alloc(" in b or "mali_va_align(" in b:
        errs.append("VA chosen outside mali_va_alloc_map")
    if MFLAGS in b and CALL in b and b.index(MFLAGS) > b.index(CALL):
        errs.append("translation after the call")
    if not re.search(r"if \(ae != 0\) \{\n\t\tlck_mtx_unlock\(&sm_lock\);\n\t\tif \(ae == -1\) \{\n\t\t\tprintf\([^;]*\);\n\t\t\treturn EINVAL;\n\t\t\}\n\t\treturn ENOMEM;\n\t\}", b):
        errs.append("failure branch not: unlock, -1 -> EINVAL, else ENOMEM")
    maps = re.findall(r"mali_pt_map\(&sm\.pt,[^;]*;", b)
    if not maps or any(not m.endswith(", mflags);") for m in maps):
        errs.append("a mali_pt_map call does not pass mflags")
    return errs


def source():
    return apply_chain(SRC, (PATCH0104, PATCH0107, PATCH0109, PATCH0149))


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

    def test_header_matches_build_sh(self):
        """The header under test is what shim/build.sh gives the host test (0104 new file + the 0149 hunk)."""
        self.assertIn("mali_va_alloc_map(", header())

    def test_source_chain_is_complete(self):
        """0109's hunk (no 'diff --git' header) is in the text the pins read."""
        self.assertIn("sm_gic_probe(void)", source())
        with self.assertRaises(ValueError):
            file_diff(PATCH0149, "bsd/dev/not_touched.c")

    def test_map_source_pins_hold(self):
        self.assertEqual(pin_errors(source()), [])

    def test_every_source_mutant_breaks_a_pin(self):
        base = source()
        for name, subs in SRC_MUTANTS:
            text = base
            for old, new in subs:
                if old not in text:
                    self.fail("source mutant %r: pattern not found: %r" % (name, old))
                text = text.replace(old, new)
            self.assertNotEqual(pin_errors(text), [], "SURVIVOR: %s" % name)


if __name__ == "__main__":
    unittest.main()
