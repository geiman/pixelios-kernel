"""Mutation test of the pure sec_ts protocol header of xnu patch 0146 (bsd/dev/sweet_sec_ts.h, raven touch; review 1 F4, CR-02).

Each framing / decode / guard rule is removed or weakened in a copy of the header (every occurrence of the pattern); the host test
shim/sweet_sec_ts_host_test.c is built against the copy and must FAIL. A mutant that still passes is a survivor and fails this test, unless it
is listed EQUIVALENT with the clause that implies it. A pattern that matches nothing fails too (the header changed under the list).
The header is extracted from the patch the same way shim/build.sh does, so the kernel's bytes are what is mutated. Same runner shape as
tools/test_wlan_wr_mutants.py (tools/mutate.py, named by CR-02, does not exist yet).
Run: python3 -B tools/test_sec_ts_mutants.py  (also picked up by `unittest discover -s tools -p 'test_*.py'`)."""
import concurrent.futures
import os
import resource
import subprocess
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PATCH = os.path.join(ROOT, "xnu", "patches", "0146-sweet-gs101-touch-sec-ts.patch")
TEST = os.path.join(ROOT, "shim", "sweet_sec_ts_host_test.c")
HDR = "bsd/dev/sweet_sec_ts.h"


def new_file(patch, path):
    """The text of one new-file hunk (same rule as the awk line in shim/build.sh: from '+++ b/path' to the next '--- ')."""
    out, on = [], False
    with open(patch, encoding="utf-8") as fh:
        for line in fh.read().split("\n"):
            if line == "+++ b/" + path:
                on = True
                continue
            if on and line.startswith("--- "):
                break
            if on and line.startswith("@@"):
                continue
            if on and line.startswith("+"):
                out.append(line[1:])
    if not out:
        raise ValueError("no new-file hunk for " + path)
    return "\n".join(out) + "\n"


# (name, [(old, new), ...], equivalent-clause or None). Every occurrence of old is replaced.
MUTANTS = [
    # --- request / write framing (sec_ts.c:162-183, 310-328)
    ("read request: byte 2 not 0x01", [("\tf[2] = 0x01;", "\tf[2] = 0x00;")], None),
    ("read request: checksum over 5 bytes", [("for (i = 0; i < 6u; i++) {", "for (i = 0; i < 5u; i++) {")], None),
    ("read request: no trailing zero", [("\tf[7] = 0x00;", "\tf[7] = 0x55;")], None),
    ("reply length not rounded to 4", [("(SEC_TS_RD_HDR + len + 1u + 3u) & ~3u", "(SEC_TS_RD_HDR + len + 1u)")], None),
    ("write: length field is len, not len + 1", [("f[2] = (uint8_t)(len + 1u);", "f[2] = (uint8_t)len;")], None),
    ("write: 9 data bytes accepted", [("if (len > SEC_TS_MAX_WRITE_DATA) {", "if (len > SEC_TS_MAX_WRITE_DATA + 1u) {")], None),
    ("write: 8 data bytes refused", [("if (len > SEC_TS_MAX_WRITE_DATA) {", "if (len >= SEC_TS_MAX_WRITE_DATA) {")], None),
    ("write: no zero padding", [("while (n & 3u) {", "while (0) {")], None),
    ("write: checksum excludes the register", [("n = SEC_TS_HDR + 1u + len;\n\tfor (i = 0; i < n; i++) {", "n = SEC_TS_HDR + 1u + len;\n\tfor (i = 0; i < n - 1u - len; i++) {")], None),
    # --- reply acceptance (sec_ts.c:411-435)
    ("reply: sync byte not checked", [("r[0] == SEC_TS_SYNC && ", "")], None),
    ("reply: register echo not checked", [("r[5] == (uint8_t)reg && ", "")], None),
    ("reply: checksum not checked", [("(uint8_t)sum == r[SEC_TS_RD_HDR + len]", "1")], None),
    ("reply: checksum over the header only", [("for (i = 0; i < SEC_TS_RD_HDR + len; i++) {\n\t\tsum += r[i];", "for (i = 0; i < SEC_TS_RD_HDR; i++) {\n\t\tsum += r[i];")], None),
    # --- status events (sec_ts.c:1096-1110, 3192-3240)
    ("boot complete: VENDOR_INFO not accepted", [("type == SEC_TS_STYPE_INFO || type == SEC_TS_STYPE_VENDOR_INFO", "type == SEC_TS_STYPE_INFO")], None),
    ("boot complete: ack byte not checked", [("&& e[1] == SEC_TS_ACK_BOOT_COMPLETE;", ";")], None),
    ("watchdog: status_data_1 not checked", [("e[2] == SEC_TS_BOOT_WATCHDOG", "1")], None),
    ("watchdog: status event id not checked", [("return (e[0] & 3u) == SEC_TS_EID_STATUS && ((e[0] >> 2) & 0xfu) == SEC_TS_STYPE_INFO &&", "return ((e[0] >> 2) & 0xfu) == SEC_TS_STYPE_INFO &&")], None),
    ("queue full: status id not checked", [("e[1] == SEC_TS_ERR_QUEUE_FULL", "1")], None),
    ("queue full: ERR type not checked", [("((e[0] >> 2) & 0xfu) == SEC_TS_STYPE_ERR &&", "")], None),
    ("queue full: event id not checked", [("return (e[0] & 3u) == SEC_TS_EID_STATUS && ((e[0] >> 2) & 0xfu) == SEC_TS_STYPE_ERR &&", "return ((e[0] >> 2) & 0xfu) == SEC_TS_STYPE_ERR &&")], None),
    # --- event counts (sec_ts.c:3140-3157)
    ("left events: ttype bits not masked", [("e[7] & 0x3fu", "e[7]")], None),
    ("left events: 31 is an overflow", [("return n > SEC_TS_MAX_EVENTS - 1u ? SEC_TS_MAX_EVENTS : n;", "return n > SEC_TS_MAX_EVENTS - 2u ? SEC_TS_MAX_EVENTS : n;")], None),
    ("left events: overflow bound one higher", [("return n > SEC_TS_MAX_EVENTS - 1u ? SEC_TS_MAX_EVENTS : n;", "return n > SEC_TS_MAX_EVENTS ? SEC_TS_MAX_EVENTS : n;")],
     "for n == 32 both versions return n == SEC_TS_MAX_EVENTS; for n > 32 both return SEC_TS_MAX_EVENTS (review 1 agreed)"),
    # --- coordinate decode (sec_ts.h:832-845, sec_ts.c:2017-2033)
    ("x low nibble from the wrong half", [("r.x = ((int)e[1] << 4) | (e[3] >> 4);", "r.x = ((int)e[1] << 4) | (e[3] & 0x0fu);")], None),
    ("y low nibble from the wrong half", [("r.y = ((int)e[2] << 4) | (e[3] & 0x0fu);", "r.y = ((int)e[2] << 4) | (e[3] >> 4);")], None),
    ("ttype low bits from byte 6", [("(unsigned)(e[7] >> 6)", "(unsigned)(e[6] >> 6)")], None),
    ("tid 10 accepted", [("tid - 1u >= SEC_TS_TOUCH_ID_MAX", "tid - 1u > SEC_TS_TOUCH_ID_MAX")], None),
    ("tid 0 not rejected explicitly", [("if (tid == 0 || tid - 1u >= SEC_TS_TOUCH_ID_MAX) {", "if (tid - 1u >= SEC_TS_TOUCH_ID_MAX) {")],
     "tid is unsigned: tid 0 makes tid - 1u == UINT_MAX, which the remaining >= SEC_TS_TOUCH_ID_MAX clause rejects"),
    ("action bits from the tid field", [("action = (e[0] >> 6) & 3u;", "action = (e[0] >> 2) & 3u;")], None),
    ("empty buffer not detected", [("if (e[0] == 0) {\n\t\tr.kind = SEC_TS_NONE;", "if (0) {\n\t\tr.kind = SEC_TS_NONE;")], None),
    ("status events decoded as coordinates", [("if ((e[0] & 3u) == SEC_TS_EID_STATUS) {\n\t\tr.kind = SEC_TS_STATUS;", "if (0) {\n\t\tr.kind = SEC_TS_STATUS;")], None),
    ("gesture event ids decoded as coordinates", [("if ((e[0] & 3u) != SEC_TS_EID_COORDINATE) {\n\t\treturn r;", "if (0) {\n\t\treturn r;")], None),
    # --- type policy (sec_ts.c:2059-2068, 2070-2095, 2148-2153; review 1 F2)
    ("unreported types not filtered (F2)", [("if (r.ttype != SEC_TS_TT_NORMAL && r.ttype != SEC_TS_TT_GLOVE && r.ttype != SEC_TS_TT_WET &&\n\t    r.ttype != SEC_TS_TT_PALM && r.ttype != SEC_TS_TT_GRIP) {", "if (0) {")], None),
    ("GLOVE not reported", [("r.ttype != SEC_TS_TT_NORMAL && r.ttype != SEC_TS_TT_GLOVE &&", "r.ttype != SEC_TS_TT_NORMAL &&")], None),
    ("WET not reported", [("r.ttype != SEC_TS_TT_GLOVE && r.ttype != SEC_TS_TT_WET &&", "r.ttype != SEC_TS_TT_GLOVE &&")], None),
    ("release of a reported type ignored", [("\t\tr.kind = SEC_TS_UP;", "\t\tr.kind = SEC_TS_IGNORED;")], None),
    ("palm / grip reported as a finger", [("(r.ttype == SEC_TS_TT_PALM || r.ttype == SEC_TS_TT_GRIP) ? SEC_TS_CANCEL : SEC_TS_DOWN", "SEC_TS_DOWN")], None),
    ("grip reported as a finger", [("(r.ttype == SEC_TS_TT_PALM || r.ttype == SEC_TS_TT_GRIP) ? SEC_TS_CANCEL", "(r.ttype == SEC_TS_TT_PALM) ? SEC_TS_CANCEL")], None),
    ("ACTION_NONE reported as a touch", [("} else if (action == SEC_TS_ACTION_PRESS || action == SEC_TS_ACTION_MOVE) {", "} else {")], None),
    # --- panel info (sec_ts.c:4112, 4125-4130)
    ("panel x bytes swapped", [("*x = ((unsigned)d[0] << 8) | d[1];", "*x = ((unsigned)d[1] << 8) | d[0];")], None),
    ("panel y from the x bytes", [("*y = ((unsigned)d[2] << 8) | d[3];", "*y = ((unsigned)d[0] << 8) | d[1];")], None),
]


def run_bounded(exe, d):
    """Run a test binary with output capped by RLIMIT_FSIZE (1 MiB), as test_wlan_wr_mutants.run_bounded."""
    def cap():
        resource.setrlimit(resource.RLIMIT_FSIZE, (1 << 20, 1 << 20))
    with open(os.path.join(d, "out.txt"), "wb") as fo, open(os.path.join(d, "err.txt"), "wb") as fe:
        rc = subprocess.run([exe], stdout=fo, stderr=fe, timeout=60, preexec_fn=cap).returncode
    with open(os.path.join(d, "err.txt"), "rb") as f:
        return rc, f.read()[-300:].decode("utf-8", "replace")


def build_and_run(hdr, work):
    d = tempfile.mkdtemp(dir=work)
    with open(os.path.join(d, "sweet_sec_ts.h"), "w", encoding="utf-8") as fh:
        fh.write(hdr)
    exe = os.path.join(d, "t")
    cc = subprocess.run(["gcc", "-O1", "-Wall", "-Wextra", "-I" + d, "-o", exe, TEST], capture_output=True, text=True, timeout=120)
    if cc.returncode != 0:
        return "compile", cc.stderr[-400:]
    rc, err = run_bounded(exe, d)
    return ("fail" if rc != 0 else "pass"), err


def run_all():
    hdr = new_file(PATCH, HDR)
    out = {}
    os.makedirs(os.path.join(ROOT, "shim", "out"), exist_ok=True)
    with tempfile.TemporaryDirectory(dir=os.path.join(ROOT, "shim", "out")) as work:
        base = build_and_run(hdr, work)
        jobs = {}
        with concurrent.futures.ThreadPoolExecutor(max_workers=8) as ex:
            for name, subs, equiv in MUTANTS:
                m = hdr
                miss = [o for o, _n in subs if o not in m]
                if miss:
                    out[name] = ("nomatch", repr(miss[0])[:120], equiv)
                    continue
                for o, n in subs:
                    m = m.replace(o, n)
                if m == hdr:
                    out[name] = ("nochange", "", equiv)
                    continue
                jobs[name] = (ex.submit(build_and_run, m, work), equiv)
            for name, (fut, equiv) in jobs.items():
                st, msg = fut.result()
                out[name] = (st, msg, equiv)
    return base, out


class SecTsMutants(unittest.TestCase):
    def test_every_rule_is_killed(self):
        base, out = run_all()
        self.assertEqual(base[0], "pass", "the unmutated header must pass the host test: " + base[1])
        bad = []
        for name, (st, msg, equiv) in out.items():
            if st == "fail" and not equiv:
                continue
            if st == "pass" and equiv:
                continue
            if st == "fail" and equiv:
                bad.append("%s: listed EQUIVALENT but killed (drop the clause)" % name)
                continue
            bad.append("%s: %s %s" % (name, "SURVIVED" if st == "pass" else st, msg))
        self.assertEqual(bad, [], "\n".join(bad))
        self.assertEqual(len(out), len(MUTANTS))
        self.assertEqual(len({n for n, _s, _e in MUTANTS}), len(MUTANTS), "duplicate mutant names")

    def test_extraction_matches_build_sh(self):
        # the same awk rule shim/build.sh runs: kernel bytes == tested bytes == mutated bytes
        awk = subprocess.run(["awk", '$0=="+++ b/bsd/dev/sweet_sec_ts.h"{on=1;next} on&&/^--- /{exit} on&&/^@@/{next} '
                              'on&&/^\\+/{print substr($0,2)}', PATCH], capture_output=True, text=True, check=True)
        self.assertEqual(awk.stdout, new_file(PATCH, HDR))

    def test_extraction_rejects_a_missing_hunk(self):
        with self.assertRaises(ValueError):
            new_file(PATCH, "bsd/dev/no_such_file.h")


if __name__ == "__main__":
    unittest.main()
