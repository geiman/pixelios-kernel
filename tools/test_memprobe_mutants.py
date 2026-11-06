"""Mutation test of shim/memprobe.c (the read-only HSI2 S2MPU dump, --mem-probe; docs/research/44; CR-02).

Each rule the verdict depends on is weakened in a copy of memprobe.c (every occurrence of the pattern); shim/memprobe_host_test.c is built against the
copy with the real raven board header and must FAIL. A survivor fails this test unless listed EQUIVALENT with its clause; a pattern that matches nothing
fails too. Same runner shape as tools/test_sec_ts_mutants.py.
Run: python3 -B tools/test_memprobe_mutants.py  (also picked up by `unittest discover -s tools -p 'test_*.py'`)."""
import concurrent.futures
import os
import resource
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SHIM = os.path.join(ROOT, "shim")
sys.path.insert(0, ROOT)
from tools import boards  # noqa: E402

MUTANTS = [
    ("enable bit read from bit 1", [("(ctrl0 & 1u)", "(ctrl0 & 2u)")], None),
    ("verdict ignores 2nd-level tables", [("v0l2 == 0)", "1)")], None),
    ("RD alone counts as open", [("(v & A_RDWR) == A_RDWR", "(v & A_RDWR)")], None),
    ("only 2 VIDs dumped", [("#define S_VIDS\t\t8u", "#define S_VIDS\t\t2u")], None),
    ("VID stride 0x100", [("0x4004u + (n) * 0x200u", "0x4004u + (n) * 0x100u")], None),
    ("granule stride 4", [("+ (m) * 8u)", "+ (m) * 4u)")], None),
    ("high mask not shifted to the bank", [("((1ull << nhi) - 1) << MEM_HIGH_GB_FIRST", "((1ull << nhi) - 1)")], None),
    ("2nd-level bit ignored", [("if (v & A_L2EN)", "if (0)")], None),
    ("unreadable entries not counted", [("bad++;", ";")], None),
    ("unreadable CTRL0 does not stop", [("\t\temit(line, p);\n\t\treturn;\n\t}\n\trd(\"CTRL1\"", "\t\temit(line, p);\n\t}\n\trd(\"CTRL1\"")], None),
    ("any open granule is enough", [("v0hi == ((1ull << nhi) - 1)", "v0hi != 0")], None),
    ("ON reported as OFF", [("if (!(ctrl0 & 1u))", "if (ctrl0 & 1u)")], None),
    ("one granule fewer in the bank", [("const unsigned nhi = MEM_HIGH_GB_LAST - MEM_HIGH_GB_FIRST + 1u;", "const unsigned nhi = MEM_HIGH_GB_LAST - MEM_HIGH_GB_FIRST;")], None),
]


def run_bounded(exe, d):
    def cap():
        resource.setrlimit(resource.RLIMIT_FSIZE, (1 << 20, 1 << 20))
    with open(os.path.join(d, "out.txt"), "wb") as fo, open(os.path.join(d, "err.txt"), "wb") as fe:
        rc = subprocess.run([exe], stdout=fo, stderr=fe, timeout=60, preexec_fn=cap).returncode
    with open(os.path.join(d, "err.txt"), "rb") as f:
        return rc, f.read()[-300:].decode("utf-8", "replace")


def build_and_run(src, work, header):
    d = tempfile.mkdtemp(dir=work)
    for n in ("memprobe.h", "memprobe_host_test.c", "console.h", "ramlog.h"):
        shutil.copy(os.path.join(SHIM, n), d)
    with open(os.path.join(d, "memprobe.c"), "w", encoding="utf-8") as fh:
        fh.write(src)
    exe = os.path.join(d, "t")
    # no -Werror: a mutant may warn (and Apple clang does not know gcc's noipa); the test binary decides
    cc = subprocess.run(["gcc", "-O1", "-Wall", "-Wextra", "-I" + d, "-include", header, "-o", exe,
                         os.path.join(d, "memprobe_host_test.c")], capture_output=True, text=True, timeout=120)
    if cc.returncode != 0:
        return "compile", cc.stderr[-400:]
    rc, err = run_bounded(exe, d)
    return ("fail" if rc != 0 else "pass"), err


def run_all():
    with open(os.path.join(SHIM, "memprobe.c"), encoding="utf-8") as fh:
        src = fh.read()
    out = {}
    os.makedirs(os.path.join(SHIM, "out"), exist_ok=True)
    with tempfile.TemporaryDirectory(dir=os.path.join(SHIM, "out")) as work:
        header = os.path.join(work, "board_config.h")
        with open(header, "w", encoding="utf-8") as fh:
            fh.write(boards.c_header("raven"))
        base = build_and_run(src, work, header)
        jobs = {}
        with concurrent.futures.ThreadPoolExecutor(max_workers=8) as ex:
            for name, subs, equiv in MUTANTS:
                m = src
                miss = [o for o, _n in subs if o not in m]
                if miss:
                    out[name] = ("nomatch", repr(miss[0])[:120], equiv)
                    continue
                for o, n in subs:
                    m = m.replace(o, n)
                jobs[name] = (ex.submit(build_and_run, m, work, header), equiv)
            for name, (fut, equiv) in jobs.items():
                st, msg = fut.result()
                out[name] = (st, msg, equiv)
    return base, out


class MemprobeMutants(unittest.TestCase):
    def test_every_rule_is_killed(self):
        base, out = run_all()
        self.assertEqual(base[0], "pass", "the unmutated memprobe.c must pass its host test: " + base[1])
        bad = []
        for name, (st, msg, equiv) in out.items():
            if st == "fail" and not equiv:
                continue
            if st == "pass" and equiv:
                continue
            bad.append("%s: %s %s" % (name, "SURVIVED" if st == "pass" else st, msg))
        self.assertEqual(bad, [], "\n".join(bad))
        self.assertEqual(len(out), len(MUTANTS))


if __name__ == "__main__":
    unittest.main()
